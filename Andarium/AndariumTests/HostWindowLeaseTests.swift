// SPDX-License-Identifier: GPL-2.0-or-later

import Testing
import UIKit
@testable import Andarium

/// The host window comes back to the next test as the application had it,
/// whatever the test that held it left on it.
@MainActor
@Suite(.serialized)
struct HostWindowLeaseTests {
    @Test func theWindowComesBackWithTheBackingItHadWhenItWasTaken() async throws {
        let first = try await HostWindow.take()
        let before = first.window.backgroundColor
        // What a root paints behind the guest's display, and what a test that
        // is done with the window in the middle of a guest phase leaves there.
        first.window.backgroundColor = .magenta
        first.give()

        let second = try await HostWindow.take()
        defer { second.give() }
        #expect(second.window.backgroundColor == before)
    }
}
