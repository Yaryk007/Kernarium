// SPDX-License-Identifier: GPL-2.0-or-later
import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The System sheet's VRAM row while Camera is on, drawn in a window as Home
/// presents it: the pool is the budget less the memory a live session's
/// cameras hold, which is the figure the runtime answered for this device's
/// cameras, and only while the session is live. SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`, as `Kernarium/
/// scripts/run-ios-simulator-tests.sh` sets it); a run without it finds none
/// and the tests fail, for they check nothing then.
@MainActor
struct SystemSheetCameraTests {
    /// An iPad mini 6's budget (534 MiB), with the ceiling (197 MiB) as the policy's own reserve.
    private let host = HostDevice(
        name: "iPad", processorCount: 1,
        memory: MemoryPolicy(
            processorCount: 1, bound: { _ in 4096 }, defaultMemory: { _ in 2048 },
            graphicsMemory: { _, _ in 534 }, cameraReserve: { _ in 197 }),
        freeStorageBytes: 40_000_000_000)

    private func model(_ access: CameraAccess, chosen: Bool, reserve: Int? = nil, exact: Bool = true) async -> (AppModel, FakeCamera) {
        let system = SystemInfo(
            version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1")
        var guest = Guest.fresh(on: host)
        guest.camera = chosen
        let model = AppModel(guest: guest, host: host, system: system, capabilities: .device, createRequirementBytes: 1)
        model.driver = StubDriver(plan: .normal)
        model.openSettings = {}
        let fake = FakeCamera(access)
        model.camera = CameraController(model: model, source: fake)
        model.cameraAccess = access
        if let reserve {
            model.cameraReservePlanner = { policy in
                CameraReservePlan(policy: policy.withCameraReserve { _ in reserve }, exact: exact)
            }
            await model.refreshCameraReserve().value
        }
        return (model, fake)
    }

    private func find(in node: NSObject, depth: Int = 0, _ match: (NSObject) -> Bool) -> NSObject? {
        if match(node) { return node }
        guard depth < 64 else { return nil }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        for child in children {
            if let found = find(in: child, depth: depth + 1, match) { return found }
        }
        return nil
    }

    /// The VRAM row's figure on the sheet, waiting for SwiftUI to publish it; nil if it never does.
    private func vram(of model: AppModel, in lease: HostWindow.Lease) async throws -> String? {
        let controller = UIHostingController(rootView: SystemSheet().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        let deadline = ContinuousClock.now + .seconds(5)
        while ContinuousClock.now < deadline {
            controller.view.layoutIfNeeded()
            if let row = find(in: controller.view, { ($0.accessibilityLabel ?? "").hasPrefix("\u{2248}") }) {
                return row.accessibilityLabel
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        return nil
    }

    @Test func theSheetShowsTheBudgetLessWhatTheRuntimeSaidWhileTheSessionIsLive() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        // Camera on and allowed: the layout the runtime would compose, 171 MiB (one stream and a RAW still).
        let live = await model(.authorized, chosen: true, reserve: 171)
        #expect(live.0.cameraMode == .live)
        #expect(try await vram(of: live.0, in: lease) == "\u{2248} 363 MiB")
        // An answer that is not the layout's own (the policy's reserve is 197 and the answer 205, so the figure
        // can only be this if the plan was applied): shown as answered.
        let notExact = await model(.authorized, chosen: true, reserve: 205, exact: false)
        #expect(try await vram(of: notExact.0, in: lease) == "\u{2248} 329 MiB")
        // A host whose cameras the image cannot describe holds nothing.
        let none = await model(.authorized, chosen: true, reserve: 0)
        #expect(try await vram(of: none.0, in: lease) == "\u{2248} 534 MiB")
    }

    @Test func theSheetShowsTheWholeBudgetWhereNoSessionIsLive() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        // The reserve is known, but the switch is off, so the session is blocked and holds nothing.
        let off = await model(.authorized, chosen: false, reserve: 171)
        #expect(off.0.cameraMode == .blocked)
        #expect(try await vram(of: off.0, in: lease) == "\u{2248} 534 MiB")
        // The question is on the screen and unanswered: blocked until the person allows it.
        let asking = await model(.notDetermined, chosen: false)
        asking.0.cameraReservePlanner = { policy in CameraReservePlan(policy: policy.withCameraReserve { _ in 171 }, exact: true) }
        asking.0.setCamera(true)
        await asking.0.cameraReserveTask?.value
        #expect(asking.0.cameraMode == .blocked)
        #expect(try await vram(of: asking.0, in: lease) == "\u{2248} 534 MiB")
        // The person allows it: the exact figure replaces the budget.
        asking.1.answer(.authorized)
        await asking.0.cameraReserveTask?.value
        #expect(asking.0.cameraMode == .live)
        #expect(try await vram(of: asking.0, in: lease) == "\u{2248} 363 MiB")
        // Refused or restricted access takes the choice back; a device without a camera keeps it, blocked.
        for access in [CameraAccess.denied, .restricted, .unavailable] {
            let blocked = await model(access, chosen: true, reserve: 171)
            #expect(blocked.0.cameraMode == .blocked, "\(access)")
            #expect(try await vram(of: blocked.0, in: lease) == "\u{2248} 534 MiB", "\(access)")
        }
    }

    @Test func theSheetAsksTheRuntimeAgainWhenItAppears() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let live = await model(.authorized, chosen: true, reserve: 171)
        #expect(try await vram(of: live.0, in: lease) == "\u{2248} 363 MiB")
        // A camera came or went without a change of access: the runtime now answers another figure.
        live.0.cameraReservePlanner = { policy in
            CameraReservePlan(policy: policy.withCameraReserve { _ in 124 }, exact: true)
        }
        // Opening the sheet asks, so the figure the person reads is the new one.
        _ = try await vram(of: live.0, in: lease)
        await live.0.cameraReserveTask?.value
        #expect(try await vram(of: live.0, in: lease) == "\u{2248} 410 MiB")
    }
}
