// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// Whether Android is ready for an app to be opened after a start. Android's
/// boot is reported done (`apexd` marks it) before the activity manager has
/// finished `sys.boot_completed`, before the home screen has taken its first
/// focus (an activity started that early is pushed behind it) and, on a first
/// start, while Android's own setup is on the screen. The request waits for all
/// of these, by asking Android, not by guessing a delay.
public enum AndroidAppReadiness {
    /// One look at Android.
    public enum State: Equatable, Sendable {
        case ready
        /// Something is still starting.
        case notYet
        /// Android's first-run setup is on the screen: an app opened now would
        /// land in it, and nothing here knows when the person will be done.
        case setupUnfinished
    }

    /// How the wait ended.
    public enum Outcome: Equatable, Sendable {
        case ready
        case setupUnfinished
        /// Android did not become ready within the limit.
        case timedOut
    }

    /// How often Android is looked at, and how long Android has to become
    /// ready (counted in the guest's own running time).
    public static let pollInterval = Duration.milliseconds(400)
    public static let limit = Duration.seconds(30)

    static let bootCompletedCommand = "getprop sys.boot_completed"
    static let homeCommand = "cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.HOME"
    /// The first-run setup, for the user who is in the foreground (the shell's
    /// own default is the system user).
    static let setupCommand = "settings --user current get secure user_setup_complete"
    /// Android's activity manager, whose resumed activity names what is in front.
    static let resumedCommand = "dumpsys activity activities"

    /// One look: the boot is complete, the internal storage is up, a home
    /// activity resolves, the first-run setup of the foreground user is done
    /// (`1`; `0` is unfinished and says so), and that home activity has
    /// resumed. Every question has to be asked and answered: a command that
    /// cannot be asked, fails, or answers anything else is "not yet".
    ///
    /// The home activity's first resume is Android's own start being over (an
    /// activity started before it is pushed behind it). For a caller that need
    /// not wait for it (`homeReached`: the start is over, or was not the
    /// caller's), another app in front is the normal state of a running
    /// Android: that look is not made, and every other question still is.
    public static func check(
        shell: any GuestShell, storageReady: () async -> Bool?, homeReached: Bool = false
    ) async -> State {
        guard let boot = try? await shell.run(bootCompletedCommand, limit: .seconds(5)), boot.exitCode == 0,
              boot.text.trimmingCharacters(in: .whitespacesAndNewlines) == "1"
        else { return .notYet }
        guard await storageReady() == true else { return .notYet }
        guard let home = try? await shell.run(homeCommand, limit: .seconds(5)), home.exitCode == 0,
              let homePackage = homePackage(in: home.text)
        else { return .notYet }
        guard let setup = try? await shell.run(setupCommand, limit: .seconds(5)), setup.exitCode == 0 else { return .notYet }
        switch setup.text.trimmingCharacters(in: .whitespacesAndNewlines) {
        case "1": break
        case "0": return .setupUnfinished
        default: return .notYet
        }
        if homeReached { return .ready }
        guard let activities = try? await shell.run(resumedCommand, limit: .seconds(10)), activities.exitCode == 0,
              resumed(homePackage, in: activities.text)
        else { return .notYet }
        return .ready
    }

    /// The package of the home activity `cmd package resolve-activity --brief`
    /// prints: its last line is `package/class`.
    static func homePackage(in text: String) -> String? {
        guard let line = text.split(whereSeparator: \.isNewline).last?.trimmingCharacters(in: .whitespaces),
              let slash = line.firstIndex(of: "/")
        else { return nil }
        let package = String(line[..<slash])
        return AndroidComponent.isPackageName(package) ? package : nil
    }

    /// Whether the activity manager says an activity of `package` is the
    /// resumed one, in any of the lines that name it (`ResumedActivity`,
    /// `topResumedActivity`, `mResumedActivity`).
    static func resumed(_ package: String, in text: String) -> Bool {
        text.split(whereSeparator: \.isNewline).contains { line in
            line.contains("esumedActivity") && (line.contains(" \(package)/") || line.contains("{\(package)/"))
        }
    }

    /// Looks every `pollInterval` until Android is ready, its setup is on the
    /// screen, or `limit` of the guest's running time has passed (`now` is
    /// that clock, so the time Andarium spends in the background does not
    /// count). Ready is what a look observed: nothing is waited for blind.
    public static func wait(
        check: () async -> State, now: () -> Duration, sleep: (Duration) async -> Void,
        limit: Duration = AndroidAppReadiness.limit, interval: Duration = pollInterval
    ) async -> Outcome {
        let started = now()
        while true {
            if Task.isCancelled { return .timedOut }
            switch await check() {
            case .setupUnfinished:
                return .setupUnfinished
            case .ready:
                return .ready
            case .notYet:
                break
            }
            if now() - started >= limit { return .timedOut }
            await sleep(interval)
        }
    }
}

/// Settings > Apps > Add to Home Screen's switch for Search: whether Android's
/// apps are offered to Spotlight. On until the person turns it off; it lasts
/// across launches. The Shortcuts tiles, the entities and the Siri phrase do
/// not follow it. It also keeps what the index was last made to hold, so that
/// a launch can finish a write that did not.
public struct AndroidAppSearchPreference {
    public static let key = "andarium.androidAppsInSearch"
    public static let indexedKey = "andarium.androidAppsIndexed"

    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    public var isOn: Bool { defaults.object(forKey: Self.key) as? Bool ?? true }

    public func keep(_ on: Bool) {
        defaults.set(on, forKey: Self.key)
    }

    /// What Spotlight was last made to hold, as the writer names it; nil when it never was.
    public var indexed: String? { defaults.string(forKey: Self.indexedKey) }

    public func keepIndexed(_ state: String?) {
        defaults.set(state, forKey: Self.indexedKey)
    }
}
