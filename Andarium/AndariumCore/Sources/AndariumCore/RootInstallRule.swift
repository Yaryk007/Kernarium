// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// When the application puts the official Magisk app in the guest, and what it
/// does once it has. Each choice is one constant here and the rest follows it.
public enum RootInstallRule {
    /// When an absent app is put in again.
    public enum Policy: Sendable {
        /// An absent app is installed once in a period of root being on; a
        /// person who removed it keeps it removed until root is turned off and
        /// on again. Magisk's own "Hide the Magisk app" is respected too.
        case oncePerOnPeriod
        /// At every start with root, whenever the app is absent.
        case everyRootedBoot
        /// Once for the guest, whatever the switch does after.
        case oncePerGuest
    }

    /// An absent app is installed once in a period of root being on.
    public static let policy = Policy.oncePerOnPeriod

    /// The installing start ends with the Magisk app on the guest's screen, so
    /// that its one-time setup dialog is the first thing the person meets.
    public static let opensAppAfterInstall = true

    public enum Action: Equatable, Sendable {
        case none
        case install
    }

    /// What a start with root does about the app, given what the guest says of
    /// it and the version the release carries. An app older than the carried
    /// one is always updated (`-r` keeps its data), and so is one of the
    /// carried version that the state does not say the application put in:
    /// whose app a package of that name is shows only in its signature, which
    /// the package manager checks and names when it refuses the install. A
    /// newer one is left alone, since Android refuses the downgrade. An absent
    /// one is put in by `policy`.
    public static func decide(
        presence: MagiskApp.Presence, carriedVersionCode: Int, state: RootState, policy: Policy = RootInstallRule.policy
    ) -> Action {
        switch presence {
        case .installed(let code):
            // A newer app is left alone and counts as present: `pm install -r`
            // refuses a downgrade, and the person's newer official app works
            // with the daemon.
            if code != carriedVersionCode { return code < carriedVersionCode ? .install : .none }
            return isOurs(state, carriedVersionCode: carriedVersionCode) ? .none : .install
        case .absent, .unknown:
            switch policy {
            case .oncePerOnPeriod: return state.appVersionCode == nil ? .install : .none
            case .everyRootedBoot: return .install
            case .oncePerGuest: return state.appEverHandled ? .none : .install
            }
        }
    }

    /// The state says the application put the app in, at the carried version
    /// or later, in this period of root being on.
    static func isOurs(_ state: RootState, carriedVersionCode: Int) -> Bool {
        (state.appVersionCode ?? 0) >= carriedVersionCode
    }

    /// Why an install that `decide` chose is made, for the bug report.
    public static func reason(
        presence: MagiskApp.Presence, carriedVersionCode: Int
    ) -> RootState.AppInstall.Reason {
        guard case .installed(let code) = presence else { return .absent }
        return code < carriedVersionCode ? .older : .notRecorded
    }

    /// What the state remembers after the application put the app in at
    /// `versionCode`. An app the application only saw is not recorded.
    public static func recorded(_ state: RootState, versionCode: Int) -> RootState {
        var state = state
        state.appVersionCode = max(state.appVersionCode ?? 0, versionCode)
        state.appEverHandled = true
        return state
    }
}
