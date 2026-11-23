// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import UIKit
@testable import Andarium

/// The hosted tests run inside the application, which has one scene and one
/// key window, and Swift Testing runs suites in parallel on the main actor,
/// each switching to another test wherever it awaits. A test that puts its
/// own root view controller in that window, or connects a window of its own,
/// would replace the root another test is waiting on, and restore a stale
/// root afterwards. The scene reads the preferences of the root it has at that
/// moment, so a test of a root's preferences then waits on a root nobody
/// holds, for as long as the other test keeps the window.
///
/// A test that does either takes the window with `HostWindow.take()` for its
/// whole body and gives it back with `defer`: one test at a time, with the
/// original root and the window's own backing back in place for the next. A
/// root of the application paints that backing black while a guest phase is
/// up and gives it back only when the phase ends, which a test that is done
/// with the window mid-phase never sees.
@MainActor
enum HostWindow {
    enum Failure: Error, CustomStringConvertible {
        case busy
        case noKeyWindow

        var description: String {
            switch self {
            case .busy: "the host window stayed with another test"
            case .noKeyWindow: "the application has no key window"
            }
        }
    }

    private static var held = false

    /// Waits for the window, then for the application's key window, and
    /// returns the lease on it. Both waits are bounded.
    static func take(
        timeout: Duration = .seconds(60), keyWindowTimeout: Duration = .seconds(20)
    ) async throws -> Lease {
        let deadline = ContinuousClock.now + timeout
        while held {
            guard ContinuousClock.now < deadline else { throw Failure.busy }
            try await Task.sleep(for: .milliseconds(20))
        }
        held = true
        let keyDeadline = ContinuousClock.now + keyWindowTimeout
        while true {
            if let lease = lease() { return lease }
            guard ContinuousClock.now < keyDeadline else {
                held = false
                throw Failure.noKeyWindow
            }
            do {
                try await Task.sleep(for: .milliseconds(50))
            } catch {
                held = false
                throw error
            }
        }
    }

    private static func lease() -> Lease? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene = scenes.first { $0.delegate is AndariumSceneDelegate } ?? scenes.first
        guard let scene, let window = scene.windows.first(where: \.isKeyWindow) else { return nil }
        return Lease(scene: scene, window: window)
    }

    /// The scene and its key window, as the application left them.
    @MainActor
    final class Lease {
        let scene: UIWindowScene
        let window: UIWindow
        private let original: UIViewController?
        private let originalBacking: UIColor?
        private var given = false

        fileprivate init(scene: UIWindowScene, window: UIWindow) {
            self.scene = scene
            self.window = window
            original = window.rootViewController
            originalBacking = window.backgroundColor
        }

        /// Puts the original root and the window's backing back, makes the
        /// window key again if a window the test connected took that over, and
        /// frees the window.
        func give() {
            guard !given else { return }
            given = true
            window.rootViewController = original
            window.backgroundColor = originalBacking
            if !window.isKeyWindow { window.makeKey() }
            HostWindow.held = false
        }
    }
}
