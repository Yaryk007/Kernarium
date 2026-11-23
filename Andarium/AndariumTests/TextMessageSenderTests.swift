// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import Foundation
import Testing
@testable import Andarium

/// The sender of the last text message belongs to the virtual machine's
/// choices: Erase forgets it, and an erase that failed leaves it.
@MainActor
final class TextMessageSenderTests {
    // The model of a test adopts the process's App Intent hub, which the suites
    // of Add to Home Screen replace: these take turns with them (`AndroidAppsHubLock`).
    init() async { await AndroidAppsHubLock.shared.acquire() }
    deinit { Task { await AndroidAppsHubLock.shared.release() } }

    private let host = HostDevice(
        name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    /// A model whose kept sender is in a defaults database of its own.
    private func model(withDriver: Bool = true) throws -> (AppModel, StubDriver, GuestSms.SenderMemory, () -> Void) {
        let suite = "TextMessageSenderTests-\(UUID().uuidString)"
        let defaults = try #require(UserDefaults(suiteName: suite))
        let memory = GuestSms.SenderMemory(defaults: defaults)
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .preview, createRequirementBytes: 1
        )
        model.smsSender = memory
        let driver = StubDriver(plan: .normal)
        if withDriver { model.driver = driver }
        model.refreshPlan()
        return (model, driver, memory, { defaults.removePersistentDomain(forName: suite) })
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while ContinuousClock.now < deadline {
            if condition() { return true }
            try? await Task.sleep(for: .milliseconds(10))
        }
        return condition()
    }

    @Test func eraseForgetsTheSender() async throws {
        let (model, driver, memory, cleanUp) = try model()
        defer { cleanUp() }
        memory.keep("My Bank")
        model.erase()
        #expect(await eventually { model.busy == nil && driver.erased == 1 })
        #expect(model.guest == nil)
        #expect(memory.sender == nil)
    }

    @Test func eraseOfAGuestThatTheRuntimeNeverHadForgetsItToo() async throws {
        let (model, _, memory, cleanUp) = try model(withDriver: false)
        defer { cleanUp() }
        memory.keep("12345")
        model.erase()
        #expect(model.guest == nil)
        #expect(memory.sender == nil)
    }

    @Test func anEraseThatFailedLeavesTheSender() async throws {
        let (model, driver, memory, cleanUp) = try model()
        defer { cleanUp() }
        memory.keep("My Bank")
        driver.eraseFails = true
        model.erase()
        #expect(await eventually { model.alert != nil })
        #expect(model.guest != nil)
        #expect(memory.sender == "My Bank")
    }
}
