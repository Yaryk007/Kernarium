// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation

/// The names a harness launch gives `LaunchOverrides` settings in the
/// environment (`Andarium/README.md#automation`). The `KERNARIUM_` names mean
/// what they mean in Kernarium; the `ANDARIUM_` ones are Andarium's own.
extension LaunchOverrides {
    static func environmentName(_ setting: Setting) -> String {
        switch setting {
        case .vCPUCount: "KERNARIUM_VCPU_COUNT"
        case .memoryMiB: "KERNARIUM_GUEST_MEMORY_MIB"
        case .diskGiB: "KERNARIUM_ANDROID_DISK_SIZE_GIB"
        case .displayScale: "KERNARIUM_DISPLAY_SCALE"
        case .retinaMode: "KERNARIUM_RETINA_MODE"
        case .start: "ANDARIUM_START"
        case .bootMode: "KERNARIUM_ANDROID_BOOT_MODE"
        case .erase: "ANDARIUM_ERASE"
        case .stats: "ANDARIUM_STATS"
        case .instance: "ANDARIUM_GUEST_INSTANCE"
        case .followsOrientation: "ANDARIUM_FOLLOW_ORIENTATION"
        }
    }

    init(environment: [String: String], processorCount: Int) {
        self.init(processorCount: processorCount) { environment[Self.environmentName($0)] }
    }
}

extension JitPreference {
    /// What a harness launch states about the JIT: "1" asks for the JIT
    /// engine, "0" for the gadget engine, and anything else is no setting.
    static func stated(in environment: [String: String]) -> Bool? {
        switch environment["ANDARIUM_JIT"] {
        case "1"?: true
        case "0"?: false
        default: nil
        }
    }
}

/// The thermal state a harness launch states instead of iOS's, for the
/// Simulator (which reports nominal) and for automation: `nominal`, `fair`,
/// `serious` or `critical` in `ANDARIUM_THERMAL_STATE`. Anything else is no
/// setting.
nonisolated enum ThermalOverride {
    static let environmentName = "ANDARIUM_THERMAL_STATE"

    static func stated(in environment: [String: String]) -> HostThermalState? {
        switch environment[environmentName]?.lowercased() {
        case "nominal"?: .nominal
        case "fair"?: .fair
        case "serious"?: .serious
        case "critical"?: .critical
        default: nil
        }
    }
}

/// The state of Settings' "When hot" a harness launch states for its run
/// (`ANDARIUM_HEAT_LIMIT`): "1" limits the frame rate while the device is hot
/// and "0" tells Android nothing hot, whatever the person chose; anything else
/// is no setting. The stored choice stays as it is
/// (`HeatLimitPreference.launch`).
nonisolated enum HeatLimitLaunch {
    static let environmentName = "ANDARIUM_HEAT_LIMIT"

    static func stated(in environment: [String: String]) -> Bool? {
        switch environment[environmentName] {
        case "1"?: true
        case "0"?: false
        default: nil
        }
    }
}

/// The state of Home's "120 Hz" a harness launch states for its run
/// (`ANDARIUM_HIGH_REFRESH`): "1" asks for the 120 Hz display at the start of
/// Android, where the screen can show it, and "0" does not, whatever the person
/// chose; anything else is no setting. The stored choice stays as it is
/// (`HighRefreshPreference.launch`).
nonisolated enum HighRefreshLaunch {
    static let environmentName = "ANDARIUM_HIGH_REFRESH"

    static func stated(in environment: [String: String]) -> Bool? {
        switch environment[environmentName] {
        case "1"?: true
        case "0"?: false
        default: nil
        }
    }
}

/// A harness launch's silent phone (`ANDARIUM_HAPTICS=0`): for that launch the
/// device plays none of the guest's haptics, as it does while the person's
/// Haptics switch is off, and the stored switch stays as it is. Anything else
/// is no setting, and the switch decides. It silences the application's own
/// touch feedback too (`InterfaceFeedback`), which the switch does not
/// govern. It is not a setting that `LaunchOverrides` holds, so a launch that
/// states only it is still the person's own.
nonisolated enum HapticsLaunch {
    static let environmentName = "ANDARIUM_HAPTICS"

    static func silences(in environment: [String: String]) -> Bool {
        environment[environmentName] == "0"
    }
}

/// A UI test's launch (`ANDARIUM_INPUT_PROBE=1`): the session opens at once
/// with no guest behind it, and the guest's keyboard and mouse send to a log
/// the session shows (`InputProbe`). The UI tests of the hardware keyboard and
/// mouse drive the real application with it, because the runtime is out of
/// reach of a test that runs in another process. The probe's guest registers
/// each device when it is plugged, as a kernel with the hot-plug topic does;
/// `unregistered` makes it one that never does, as a kernel without it is.
nonisolated enum InputProbeLaunch {
    static let environmentName = "ANDARIUM_INPUT_PROBE"
    static let unregisteredValue = "unregistered"

    static func requested(in environment: [String: String]) -> Bool {
        environment[environmentName] == "1" || environment[environmentName] == unregisteredValue
    }

    /// Whether the probe's guest registers the devices it is given.
    static func guestRegisters(in environment: [String: String]) -> Bool {
        environment[environmentName] != unregisteredValue
    }

    /// `ANDARIUM_INPUT_PROBE_TIP=1` leaves the side keys' tip on, newly shown
    /// at the launch, where the probe hides the tips.
    static let tipEnvironmentName = "ANDARIUM_INPUT_PROBE_TIP"

    static func showsTip(in environment: [String: String]) -> Bool {
        requested(in: environment) && environment[tipEnvironmentName] == "1"
    }
}

/// A UI test's probe session with a scripted list of old apps in place of the
/// image's `kernarium_oldapp` command (`ANDARIUM_OLD_APPS_PROBE`): `apps` lists
/// two made-up apps, one of them on and open; `refuse` lists the same and
/// refuses every change; `none` lists none, as an image without the command.
/// Settings > Apps > Old app compatibility is then reachable without a guest.
nonisolated enum OldAppsProbeLaunch {
    static let environmentName = "ANDARIUM_OLD_APPS_PROBE"

    enum Script: String {
        case apps, refuse, none
    }

    static func script(in environment: [String: String]) -> Script? {
        InputProbeLaunch.requested(in: environment) ? environment[environmentName].flatMap(Script.init(rawValue:)) : nil
    }
}

/// A probe session with a played list of Android's apps in place of the
/// image's `kernarium_apps` tool (`ANDARIUM_ANDROID_APPS_PROBE=1`): Settings >
/// Apps > Add to Home Screen is reachable without a guest, and the cache and
/// the App Shortcuts hold the made-up apps, so Apple's Shortcuts shows their
/// tiles under Andarium.
nonisolated enum AndroidAppsProbeLaunch {
    static let environmentName = "ANDARIUM_ANDROID_APPS_PROBE"

    static func requested(in environment: [String: String]) -> Bool {
        InputProbeLaunch.requested(in: environment) && environment[environmentName] == "1"
    }
}

/// The development build's compass check (`ANDARIUM_COMPASS_READOUT=1`): the
/// application prints the device motion sample the guest's field comes from,
/// with iOS's own attitude and heading from it (`CompassReadout`).
nonisolated enum CompassReadoutLaunch {
    static let environmentName = "ANDARIUM_COMPASS_READOUT"

    static func requested(in environment: [String: String]) -> Bool {
        environment[environmentName] == "1"
    }
}

extension AppEntry {
    /// Whether the launch is the person's own, which announces the release
    /// notes (`announceWhatsNew`). A launch that states any setting, the JIT
    /// or the thermal state or the heat limit or the 120 Hz display included, or runs under XCTest is a
    /// harness's.
    static func isPersonsLaunch(overrides: LaunchOverrides, environment: [String: String]) -> Bool {
        overrides.isEmpty && JitPreference.stated(in: environment) == nil
            && ThermalOverride.stated(in: environment) == nil
            && HeatLimitLaunch.stated(in: environment) == nil
            && HighRefreshLaunch.stated(in: environment) == nil
            && !InputProbeLaunch.requested(in: environment)
            && !CompassReadoutLaunch.requested(in: environment)
            && environment[OldAppsProbeLaunch.environmentName] == nil
            && environment[AndroidAppsProbeLaunch.environmentName] == nil
            && environment["XCTestConfigurationFilePath"] == nil
    }

    /// How Home's AOT row begins for a launch: the person's last choice for
    /// their own launch, and on, keeping nothing, for every launch
    /// `isPersonsLaunch` refuses, so that a harness whose only mark is an
    /// environment value (the JIT's, the thermal state's) never inherits the
    /// person's choice or stores a change.
    static func translatedLibrariesLaunch(
        overrides: LaunchOverrides, environment: [String: String],
        preference: TranslatedLibrariesPreference = TranslatedLibrariesPreference()
    ) -> TranslatedLibrariesPreference.Launch {
        preference.launch(automated: !isPersonsLaunch(overrides: overrides, environment: environment))
    }

    /// The file in an isolated guest instance's directory whose number, in
    /// mebibytes, stands for the device's free space while it exists, so that
    /// a harness launch drives the run's low-space pause without filling the
    /// disk. Only an isolated instance reads it.
    static let harnessFreeSpaceFile = "free-space-mib"

    static func harnessFreeSpace(
        in directory: URL, fallback: @escaping @Sendable () -> UInt64
    ) -> @Sendable () -> UInt64 {
        let file = directory.appendingPathComponent(harnessFreeSpaceFile)
        return {
            guard let text = try? String(contentsOf: file, encoding: .utf8),
                let mebibytes = UInt64(text.trimmingCharacters(in: .whitespacesAndNewlines))
            else { return fallback() }
            return mebibytes * Limits.bytesPerMiB
        }
    }

    /// The file in an isolated instance's directory that answers the screen
    /// over a guest paused for space as the person would: `resume`,
    /// `shutdown` or `check`. The harness has no finger, so the application
    /// takes the answer when the screen offers it, and removes the file.
    static let harnessSpaceAnswerFile = "space-answer"

    @MainActor
    static func answerSpaceScreenFromHarness(model: AppModel, directory: URL) {
        let file = directory.appendingPathComponent(harnessSpaceAnswerFile)
        Task { @MainActor [weak model] in
            while !Task.isCancelled, let model {
                try? await Task.sleep(for: .seconds(1))
                guard let text = try? String(contentsOf: file, encoding: .utf8),
                    let stage = model.spacePause?.stage, model.spacePause?.working == nil
                else { continue }
                let answer = text.trimmingCharacters(in: .whitespacesAndNewlines)
                switch (answer, stage) {
                case ("resume", .paused): model.resumeAfterSpacePause()
                case ("shutdown", .paused), ("shutdown", .notPaused): model.shutDownAfterSpacePause()
                case ("check", .notPaused): model.checkSpaceAfterPauseFailed()
                default: continue
                }
                print("Andarium: the harness answered the space screen: \(answer)")
                try? FileManager.default.removeItem(at: file)
            }
        }
    }
}
