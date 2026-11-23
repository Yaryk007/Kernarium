// SPDX-License-Identifier: GPL-2.0-or-later

/// The App Intent's hub (`AndroidAppsHub.shared`: the cache directory, the
/// attached model and the index) belongs to the process, and a test replaces
/// all three. Swift Testing runs suites side by side, so the suites that
/// replace them take turns: a test of such a suite is made holding this lock
/// (in the suite's `init`) and gives it up when it is gone (`deinit`).
actor AndroidAppsHubLock {
    static let shared = AndroidAppsHubLock()

    private var held = false
    private var waiting: [CheckedContinuation<Void, Never>] = []

    func acquire() async {
        if !held {
            held = true
            return
        }
        await withCheckedContinuation { waiting.append($0) }
    }

    func release() {
        if waiting.isEmpty {
            held = false
        } else {
            waiting.removeFirst().resume()
        }
    }
}
