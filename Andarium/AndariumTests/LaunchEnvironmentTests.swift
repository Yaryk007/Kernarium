// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import Testing

@testable import Andarium

/// The environment names a harness launch uses are the application's
/// (`Andarium/README.md#automation`).
struct LaunchEnvironmentTests {
    @Test func everySettingHasItsOwnName() {
        let names = LaunchOverrides.Setting.allCases.map { LaunchOverrides.environmentName($0) }
        #expect(Set(names).count == LaunchOverrides.Setting.allCases.count)
    }

    @Test func readsTheNamesOfKernariumAndAndarium() {
        let overrides = LaunchOverrides(
            environment: [
                "KERNARIUM_VCPU_COUNT": "2",
                "KERNARIUM_GUEST_MEMORY_MIB": "1536",
                "KERNARIUM_ANDROID_DISK_SIZE_GIB": "32",
                "KERNARIUM_DISPLAY_SCALE": "1.5",
                "ANDARIUM_START": "recovery",
                "ANDARIUM_ERASE": "1",
                "ANDARIUM_STATS": "1",
                "ANDARIUM_GUEST_INSTANCE": "legacy-test",
            ],
            processorCount: 6
        )
        #expect(overrides.vCPUCount == 2)
        #expect(overrides.memoryMiB == 1536)
        #expect(overrides.capacityGiB == 32)
        #expect(overrides.displayScale == .factor(1.5))
        #expect(overrides.start == .recovery)
        #expect(overrides.erase)
        #expect(overrides.statistics == true)
        #expect(overrides.instance == "legacy-test")
    }

    @Test func theDisplayScaleIsStatedByNameOrByTheEarlierRetinaName() {
        func stated(_ environment: [String: String]) -> GuestDisplayScale? {
            LaunchOverrides(environment: environment, processorCount: 6).displayScale
        }
        #expect(stated(["KERNARIUM_DISPLAY_SCALE": "native"]) == .native)
        #expect(stated(["KERNARIUM_DISPLAY_SCALE": "1"]) == .points)
        // What harnesses stated before the scale: on is native, off is 1.0.
        #expect(stated(["KERNARIUM_RETINA_MODE": "1"]) == .native)
        #expect(stated(["KERNARIUM_RETINA_MODE": "0"]) == .points)
        #expect(stated(["KERNARIUM_DISPLAY_SCALE": "2", "KERNARIUM_RETINA_MODE": "1"]) == .factor(2))
        #expect(stated(["KERNARIUM_DISPLAY_SCALE": "sharpest"]) == nil)
    }

    @Test func isolatedGuestsHaveSeparateStorage() {
        let ordinary = AppEntry.guestDirectory()
        let isolated = AppEntry.guestDirectory(instance: "legacy-test")
        #expect(ordinary.lastPathComponent == "Guest")
        #expect(isolated.lastPathComponent == "legacy-test")
        #expect(isolated.deletingLastPathComponent().lastPathComponent == "TestGuests")
        #expect(isolated != ordinary)
    }

    @Test func theThermalStateIsStatedByNameAndAnythingElseIsNoSetting() {
        #expect(ThermalOverride.environmentName == "ANDARIUM_THERMAL_STATE")
        for (word, state) in [
            ("nominal", HostThermalState.nominal), ("fair", .fair), ("serious", .serious), ("critical", .critical),
            ("Serious", .serious),
        ] {
            #expect(ThermalOverride.stated(in: ["ANDARIUM_THERMAL_STATE": word]) == state, "\(word)")
        }
        for word in ["", "hot", "3", "emergency", "serious "] {
            #expect(ThermalOverride.stated(in: ["ANDARIUM_THERMAL_STATE": word]) == nil, "\(word)")
        }
        #expect(ThermalOverride.stated(in: [:]) == nil)
    }

    @Test func aLaunchThatStatesTheThermalStateIsAHarnesss() {
        let none = LaunchOverrides(environment: [:], processorCount: 4)
        #expect(AppEntry.isPersonsLaunch(overrides: none, environment: [:]))
        #expect(!AppEntry.isPersonsLaunch(overrides: none, environment: ["ANDARIUM_THERMAL_STATE": "serious"]))
    }

    @Test func theHapticsSettingSilencesALaunchOnlyWhenItIsZero() {
        #expect(HapticsLaunch.environmentName == "ANDARIUM_HAPTICS")
        #expect(HapticsLaunch.silences(in: ["ANDARIUM_HAPTICS": "0"]))
        // Anything else is no setting, and the person's switch decides.
        for word in ["", "1", "off", "false", "00", "0 "] {
            #expect(!HapticsLaunch.silences(in: ["ANDARIUM_HAPTICS": word]), "\(word)")
        }
        #expect(!HapticsLaunch.silences(in: [:]))
    }

    @Test func aLaunchThatOnlySilencesHapticsIsStillThePersonsLaunch() {
        // A silent phone makes no other difference to the launch: it stays one
        // that announces the release notes, and no setting `LaunchOverrides`
        // holds, so the pointer, statistics and JIT keep the person's choices.
        let environment = ["ANDARIUM_HAPTICS": "0"]
        let overrides = LaunchOverrides(environment: environment, processorCount: 4)
        #expect(overrides.isEmpty)
        #expect(AppEntry.isPersonsLaunch(overrides: overrides, environment: environment))
    }

    @MainActor
    @Test func theModelOfASilencedLaunchPlaysNoHapticsAndKeepsTheChoice() {
        let entry = AppEntry.fromArguments()
        let silenced = entry.makeModel(environment: ["ANDARIUM_HAPTICS": "0"])
        #expect(silenced.hapticsSilencedByLaunch)
        #expect(!silenced.hapticsPlays)
        // The switch is the person's and reads as stored.
        #expect(silenced.hapticsOn)
        for environment in [[:], ["ANDARIUM_HAPTICS": "1"]] as [[String: String]] {
            let model = entry.makeModel(environment: environment)
            #expect(!model.hapticsSilencedByLaunch)
            #expect(model.hapticsPlays == model.hapticsOn)
        }
    }

    /// A UI test's input-probe launch builds its model on a path of its own.
    /// It is an automated run too, and the side keys it opens must stay as
    /// silent as the ordinary launch's: both flags together.
    @MainActor
    @Test func anInputProbeLaunchAppliesTheSilentPhoneToo() {
        let entry = AppEntry.fromArguments()
        let both = entry.makeModel(environment: ["ANDARIUM_INPUT_PROBE": "1", "ANDARIUM_HAPTICS": "0"])
        #expect(both.inputProbe != nil, "the probe's model, not the ordinary launch's")
        #expect(both.hapticsSilencedByLaunch)
        #expect(!both.interfaceFeedbackPlays)
        #expect(both.hapticsOn)
        // The probe alone silences nothing.
        for environment in [["ANDARIUM_INPUT_PROBE": "1"], ["ANDARIUM_INPUT_PROBE": "1", "ANDARIUM_HAPTICS": "1"]] {
            let probe = entry.makeModel(environment: environment)
            #expect(probe.inputProbe != nil)
            #expect(!probe.hapticsSilencedByLaunch)
            #expect(probe.interfaceFeedbackPlays)
        }
        // And the silent phone alone is not the probe.
        #expect(entry.makeModel(environment: ["ANDARIUM_HAPTICS": "0"]).inputProbe == nil)
    }

    /// A harness whose only mark is an environment value has an empty
    /// `LaunchOverrides`, yet it is not the person's launch: the AOT row
    /// starts on for it and keeps nothing, so that the person's choice is
    /// neither inherited nor overwritten.
    @Test func everyHarnessLaunchStartsTheAotRowOnAndKeepsNothing() throws {
        let name = "andarium-tests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: name))
        defer { defaults.removePersistentDomain(forName: name) }
        let preference = TranslatedLibrariesPreference(defaults: defaults)
        preference.keep(false)
        let marks: [[String: String]] = [
            ["ANDARIUM_JIT": "0"], ["ANDARIUM_JIT": "1"],
            ["ANDARIUM_THERMAL_STATE": "serious"], ["ANDARIUM_HEAT_LIMIT": "0"],
            ["ANDARIUM_INPUT_PROBE": "1"], ["ANDARIUM_OLD_APPS_PROBE": "1"],
            ["ANDARIUM_ANDROID_APPS_PROBE": "1"], ["XCTestConfigurationFilePath": "/tmp/x"],
        ]
        for environment in marks {
            let overrides = LaunchOverrides(environment: environment, processorCount: 4)
            let launch = AppEntry.translatedLibrariesLaunch(
                overrides: overrides, environment: environment, preference: preference)
            #expect(launch.isOn, "\(environment)")
            #expect(!launch.keepsChanges, "\(environment)")
        }
        // The person's own launch restores what they chose and keeps changes.
        let own = AppEntry.translatedLibrariesLaunch(
            overrides: LaunchOverrides(environment: [:], processorCount: 4), environment: [:], preference: preference)
        #expect(!own.isOn)
        #expect(own.keepsChanges)
        // A launch that states a setting through the overrides is a harness's too.
        let stated = ["KERNARIUM_ANDROID_BOOT_MODE": "recovery"]
        let recovery = AppEntry.translatedLibrariesLaunch(
            overrides: LaunchOverrides(environment: stated, processorCount: 4), environment: stated, preference: preference)
        #expect(recovery.isOn)
        #expect(!recovery.keepsChanges)
    }

    /// The same through the model the application builds, with the person's
    /// choice stored where the application reads it.
    @MainActor
    @Test func theModelOfAJitOnlyLaunchDoesNotInheritAnAotChoice() {
        let defaults = UserDefaults.standard
        let kept = defaults.object(forKey: TranslatedLibrariesPreference.key)
        defer {
            if let kept { defaults.set(kept, forKey: TranslatedLibrariesPreference.key) }
            else { defaults.removeObject(forKey: TranslatedLibrariesPreference.key) }
        }
        TranslatedLibrariesPreference(defaults: defaults).keep(false)
        let entry = AppEntry.fromArguments()
        let harness = entry.makeModel(environment: ["ANDARIUM_JIT": "0"])
        #expect(harness.translatedLibrariesOn)
        #expect(harness.translatedLibrariesDecidedByLaunch)
        #expect(harness.translatedLibrariesToKeep == nil)
        let person = entry.makeModel(environment: [:])
        #expect(!person.translatedLibrariesOn)
        #expect(!person.translatedLibrariesDecidedByLaunch)
    }

    @Test func kernariumsBootModeAsksForARecoveryStartOnly() {
        #expect(LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "recovery"], processorCount: 4).start == .recovery)
        #expect(LaunchOverrides(environment: ["KERNARIUM_ANDROID_BOOT_MODE": "normal"], processorCount: 4).start == nil)
        #expect(LaunchOverrides(environment: [:], processorCount: 4).isEmpty)
    }
}
