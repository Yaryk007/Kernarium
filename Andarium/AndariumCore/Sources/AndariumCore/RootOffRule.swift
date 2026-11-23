// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// What becomes of the Magisk app when root is off: it stays installed. The
/// behaviour is one constant here.
public enum RootOffRule {
    public enum AppPolicy: Sendable {
        /// The app stays installed: its settings and grants survive and apps
        /// that look for it can see it.
        case keep
        /// The first start with root off removes the app the application put
        /// in, and the next start with root puts it in again.
        case uninstall
    }

    /// The app stays installed.
    public static let appPolicy = AppPolicy.keep

    /// Whether a start without root asks the guest about the app to remove it.
    public static func removesApp(state: RootState, policy: AppPolicy = RootOffRule.appPolicy) -> Bool {
        policy == .uninstall && !state.on && state.appEverHandled
    }

    /// A start without root, when the rule removes the app: removes it if it
    /// is there, and says whether the guest was asked successfully. Bounded by
    /// `limit` for each command.
    public static func removeAppIfAny(shell: any GuestShell, limit: Duration) async -> Bool {
        switch await MagiskApp.presence(shell: shell, limit: limit) {
        case .absent: return true
        case .unknown: return false
        case .installed: return await MagiskApp.uninstall(shell: shell, limit: limit)
        }
    }
}
