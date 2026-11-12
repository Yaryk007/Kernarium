// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// Every sheet finds the model it reads, and Settings over a session keeps the
/// session's status bar hidden, at either height. On a Mac (Designed for iPad)
/// a sheet without either ends the application.
@MainActor
@Suite(.serialized)
struct SheetPresentationTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private func model(phase: Phase = .home) -> AppModel {
        let host = HostDevice(
            name: "iPad", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
        )
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.launchApplied = true
        model.driver = StubDriver(plan: .normal)
        model.phase = phase
        return model
    }

    /// Waits for the sheet `controller` presents to be on screen and to
    /// settle: its presentation preferences are read again after the
    /// transition.
    private func waitForSheet(of controller: UIViewController) async throws {
        let shown = await eventually {
            guard let presented = controller.presentedViewController else { return false }
            presented.view.layoutIfNeeded()
            return presented.view.window != nil
        }
        try #require(shown)
        try await Task.sleep(for: .seconds(1))
    }

    @Test(arguments: SheetKind.allCases)
    func homePresentsEverySheet(_ kind: SheetKind) async throws {
        let model = model()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        try #require(await eventually { controller.viewIfLoaded?.window != nil })
        model.sheet = kind
        try await waitForSheet(of: controller)
        model.sheet = nil
        #expect(await eventually { controller.presentedViewController == nil })
    }

    private struct SessionStandIn: View {
        let model: AppModel
        let large: Bool
        @State private var settings = false

        var body: some View {
            Color.black
                .statusBarHidden(true)
                .sessionSettings(isPresented: $settings, large: large, model: model)
                .task { settings = true }
        }
    }

    @Test(arguments: [false, true])
    func sessionSettingsKeepTheStatusBarHidden(large: Bool) async throws {
        let model = model(phase: .running(.normal))
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: SessionStandIn(model: model, large: large), model: model)
        lease.window.rootViewController = controller
        try await waitForSheet(of: controller)
        #expect(lease.scene.statusBarManager?.isStatusBarHidden == true)
        controller.dismiss(animated: false)
        #expect(await eventually { controller.presentedViewController == nil })
    }
}
