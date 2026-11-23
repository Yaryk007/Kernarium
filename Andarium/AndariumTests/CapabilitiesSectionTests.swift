// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The Capabilities section as the application draws it: the Haptics row is
/// there only on a device that has haptics, and its switch is the guest's
/// choice, kept and passed to the driver. The Simulator has no Taptic Engine,
/// so a test states the capability the way the runtime does for a device.
@MainActor
@Suite(.serialized)
struct CapabilitiesSectionTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )

    private func model(hapticsAvailable: Bool) -> (model: AppModel, driver: StubDriver) {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        let driver = StubDriver(plan: .normal)
        model.driver = driver
        model.hapticsAvailable = hapticsAvailable
        return (model, driver)
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func switches(in view: UIView) -> [UISwitch] {
        var found: [UISwitch] = []
        if let control = view as? UISwitch { found.append(control) }
        for child in view.subviews { found += switches(in: child) }
        return found
    }

    /// Hosts the section on the application's window, laid out at a phone's
    /// width, and waits for its switches to be drawn.
    private func host(_ model: AppModel, count: Int, in lease: HostWindow.Lease) async throws -> UIHostingController<some View> {
        let controller = UIHostingController(rootView: CapabilitiesSection().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let drawn = await eventually {
            controller.view.setNeedsLayout()
            controller.view.layoutIfNeeded()
            return switches(in: controller.view).count == count
        }
        try #require(drawn, "the section drew \(switches(in: controller.view).count) switches, not \(count)")
        return controller
    }

    @Test func theRowIsThereOnADeviceWithHapticsAndAbsentOnOneWithout() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let with = model(hapticsAvailable: true)
        let controller = try await host(with.model, count: 4, in: lease)
        // Location, Microphone, Camera, then Haptics: the motion sensors have no row.
        #expect(switches(in: controller.view).count == 4)
        let without = model(hapticsAvailable: false)
        let other = try await host(without.model, count: 3, in: lease)
        #expect(switches(in: other.view).count == 3)
    }

    @Test func theSwitchIsTheGuestsChoiceAndRoundTrips() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, driver) = model(hapticsAvailable: true)
        let controller = try await host(model, count: 4, in: lease)
        var haptics = try #require(switches(in: controller.view).last)
        // On for a new guest.
        #expect(haptics.isOn)

        haptics.setOn(false, animated: false)
        haptics.sendActions(for: .valueChanged)
        #expect(await eventually { model.guest?.haptics == false })
        #expect(!driver.haptics)
        #expect(driver.savedGuest?.haptics == false)
        // The other three are as they were.
        let others = switches(in: controller.view).dropLast()
        #expect(others.map(\.isOn) == [false, false, false])

        haptics = try #require(switches(in: controller.view).last)
        #expect(!haptics.isOn)
        haptics.setOn(true, animated: false)
        haptics.sendActions(for: .valueChanged)
        #expect(await eventually { model.guest?.haptics == true })
        #expect(driver.haptics)
        #expect(driver.savedGuest?.haptics == true)
    }

    @Test func theRowShowsTheChoiceItIsGiven() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let (model, _) = model(hapticsAvailable: true)
        model.guest?.haptics = false
        let controller = try await host(model, count: 4, in: lease)
        #expect(try #require(switches(in: controller.view).last).isOn == false)
    }
}
