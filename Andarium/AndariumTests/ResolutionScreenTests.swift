// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import AndariumCoreTestSupport
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The resolution row of Guest system and the sheet Change opens, as the
/// application draws them and VoiceOver reads them. SwiftUI publishes its
/// accessibility elements only while application accessibility is on (the
/// Simulator preference `ApplicationAccessibilityEnabled`, as `Kernarium/
/// scripts/run-ios-simulator-tests.sh` sets it), so a run without it fails
/// with that said.
@MainActor
@Suite(.serialized)
struct ResolutionScreenTests {
    private let host = HostDevice(
        name: "iPhone", processorCount: 6, memory: MemoryPolicy(maximumsMiB: [4096]), freeStorageBytes: 40_000_000_000
    )
    private let phone = CGSize(width: 402, height: 778)

    /// A model that states its screen and display, so the window the test
    /// runs in decides nothing.
    private func model(
        native: Double, chosen: GuestDisplayScale? = nil, points: CGSize? = nil, limit: UInt64 = 0
    ) -> AppModel {
        let model = AppModel(
            guest: Guest.fresh(on: host), host: host,
            system: SystemInfo(version: "17", release: "r2", kernel: "Linux 6.18", architecture: "arm64, 4 KiB pages", appVersion: "1"),
            capabilities: .device, createRequirementBytes: 1
        )
        model.driver = StubDriver(plan: .normal)
        model.statedNativeScale = native
        model.statedDisplayPoints = points ?? phone
        model.screenDidChange(nativeScale: native)
        model.displayPointsDidChange(points ?? phone)
        model.chosenScale = chosen
        model.memoryLimitBytes = limit
        model.launchApplied = true
        return model
    }

    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    /// Every accessibility node under `node`.
    private func nodes(in node: NSObject, depth: Int = 0) -> [NSObject] {
        guard depth < 64 else { return [] }
        var children = (node.accessibilityElements ?? []).compactMap { $0 as? NSObject }
        if let view = node as? UIView { children += view.subviews }
        return [node] + children.flatMap { nodes(in: $0, depth: depth + 1) }
    }

    /// The node labelled `label`. Before iOS 27 a bar's button is hosted in a view that has the
    /// label too and is no accessibility element, ahead of the element itself, so the one VoiceOver
    /// reaches is taken there.
    private func node(labelled label: String, in root: UIView) -> NSObject? {
        let named = nodes(in: root).filter { $0.accessibilityLabel == label }
        if #available(iOS 27.0, *) { return named.first }
        return named.first { $0.isAccessibilityElement || $0 is UIControl } ?? named.first
    }

    private func labels(in root: UIView) -> [String] {
        nodes(in: root).compactMap(\.accessibilityLabel)
    }

    /// Waits for a node with `label`, failing with the cause the Simulator's
    /// accessibility preference is the likeliest of.
    private func waitFor(_ label: String, in root: UIView) async throws -> NSObject {
        var found: NSObject?
        let seen = await eventually {
            root.setNeedsLayout()
            root.layoutIfNeeded()
            found = node(labelled: label, in: root)
            return found != nil
        }
        return try #require(
            found, "no accessibility element is labelled \(label); is ApplicationAccessibilityEnabled on? \(seen) \(labels(in: root))")
    }

    /// What VoiceOver's double tap does. A UIKit control of an iPad app on a
    /// Mac declines it, so there the control is tapped; elsewhere a control
    /// that declines it is a failure.
    private func activate(_ node: NSObject) -> Bool {
        if node.accessibilityActivate() { return true }
        guard ProcessInfo.processInfo.isiOSAppOnMac, let control = node as? UIControl else { return false }
        control.sendActions(for: .touchUpInside)
        return true
    }

    private func hostRow(_ model: AppModel, in lease: HostWindow.Lease) -> UIHostingController<some View> {
        let controller = UIHostingController(rootView: SystemSection().environment(model).frame(width: 390))
        lease.window.rootViewController = controller
        controller.view.layoutIfNeeded()
        return controller
    }

    @Test func theRowStatesThePixelsAndNamesTheEndsOnly() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        // Native, a choice between the ends, and the fastest.
        for (chosen, label) in [
            (GuestDisplayScale.native, "Resolution, 1206 by 2334, Native"),
            (.factor(1.5), "Resolution, 603 by 1167"),
            (.factor(1), "Resolution, 402 by 778, Fastest"),
        ] {
            let controller = hostRow(model(native: 3, chosen: chosen), in: lease)
            _ = try await waitFor(label, in: controller.view)
        }
    }

    @Test func theRowSaysWhatTheGuestWillGetOnAScreenOfAnotherScale() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostRow(model(native: 2.61, chosen: .native), in: lease)
        // 402 x 778 points at 2.61: each side rounded down.
        _ = try await waitFor("Resolution, 1049 by 2030, Native", in: controller.view)
        let narrow = hostRow(model(native: 2, chosen: .native, points: CGSize(width: 600, height: 800)), in: lease)
        _ = try await waitFor("Resolution, 1200 by 1600, Native", in: narrow.view)
    }

    /// Someone who never chose: 320 dpi on a device with more than 4 GiB (and a Mac), 240 dpi on one held to
    /// 4 GiB or less; an end's name only where the choice is an end.
    @Test func theRowOfSomeoneWhoNeverChoseStatesTheDefault() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let low = UInt64(2867) << 20
        let ipad = CGSize(width: 600, height: 800)
        for (native, points, limit, label) in [
            // An iPhone, native 3.0: 2.0 is not native, and 1.5 is not an end.
            (3.0, phone, UInt64(8) << 30, "Resolution, 804 by 1556"),
            (3.0, phone, UInt64(0), "Resolution, 804 by 1556"),
            (3.0, phone, low, "Resolution, 603 by 1167"),
            // An iPad mini, native 2.0: 320 dpi is native there.
            (2.0, ipad, UInt64(8) << 30, "Resolution, 1200 by 1600, Native"),
            (2.0, ipad, low, "Resolution, 900 by 1200"),
        ] {
            let controller = hostRow(model(native: native, points: points, limit: limit), in: lease)
            _ = try await waitFor(label, in: controller.view)
        }
    }

    @Test func aScreenWithOneChoiceHasNothingToChange() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = hostRow(model(native: 1), in: lease)
        // The resolution, with no name for an end.
        _ = try await waitFor("Resolution, 402 by 778", in: controller.view)
        let change = try await waitFor("Change resolution", in: controller.view)
        #expect(change.accessibilityTraits.contains(.notEnabled))
        let more = hostRow(model(native: 3), in: lease)
        let enabled = try await waitFor("Change resolution", in: more.view)
        #expect(!enabled.accessibilityTraits.contains(.notEnabled))
    }

    @Test func changeOpensTheSheet() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(native: 3)
        let controller = hostRow(model, in: lease)
        let change = try await waitFor("Change resolution", in: controller.view)
        #expect(change.accessibilityActivate())
        #expect(await eventually { model.sheet == .resolution })
    }

    /// Home on the application's window with the sheet open.
    private func openSheet(_ model: AppModel, in lease: HostWindow.Lease) async throws -> UIViewController {
        let controller = AndariumHostingController(
            rootView: RootView(entry: AppEntry(overrides: LaunchOverrides())).environment(model), model: model)
        lease.window.rootViewController = controller
        try #require(await eventually { controller.viewIfLoaded?.window != nil })
        model.sheet = .resolution
        try #require(await eventually { controller.presentedViewController?.view.window != nil })
        try await Task.sleep(for: .seconds(1))
        return try #require(controller.presentedViewController)
    }

    @Test func theSheetListsEveryChoiceSharpestFirstWithItsDensity() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(native: 3)
        let sheet = try await openSheet(model, in: lease)
        // The current choice (320 dpi, by default on a Mac or a device with more than 4 GiB) is selected.
        _ = try await waitFor("804 by 1556, 320 dpi", in: sheet.view)
        let expected = [
            "1206 by 2334, 480 dpi, Native", "1005 by 1945, 400 dpi", "804 by 1556, 320 dpi",
            "603 by 1167, 240 dpi", "402 by 778, 160 dpi, Fastest",
        ]
        let all = labels(in: sheet.view)
        let positions = expected.map { all.firstIndex(of: $0) }
        #expect(positions.allSatisfy { $0 != nil }, "\(all)")
        #expect(positions.compactMap { $0 } == positions.compactMap { $0 }.sorted(), "the sharpest is first")
        let selected = try #require(node(labelled: expected[2], in: sheet.view))
        #expect(selected.accessibilityTraits.contains(.selected))
        for index in [0, 1, 3, 4] {
            let other = try #require(node(labelled: expected[index], in: sheet.view))
            #expect(!other.accessibilityTraits.contains(.selected), "\(expected[index])")
        }
        // Nothing is chosen until Save.
        #expect(model.chosenScale == nil)
        model.sheet = nil
    }

    @Test func theSheetTicksTheLowerDefaultOnADeviceHeldToFourGiBOrLess() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(native: 3, limit: UInt64(2867) << 20)
        let sheet = try await openSheet(model, in: lease)
        let selected = try await waitFor("603 by 1167, 240 dpi", in: sheet.view)
        #expect(selected.accessibilityTraits.contains(.selected))
        let high = try #require(node(labelled: "804 by 1556, 320 dpi", in: sheet.view))
        #expect(!high.accessibilityTraits.contains(.selected))
        #expect(model.chosenScale == nil)
        model.sheet = nil
    }

    @Test func saveKeepsTheTickedChoiceAndCancelKeepsNothing() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(native: 3, chosen: .native)
        let sheet = try await openSheet(model, in: lease)
        let middle = try await waitFor("603 by 1167, 240 dpi", in: sheet.view)
        // Save is off until another choice is ticked.
        let save = try await waitFor("Save", in: sheet.view)
        #expect(save.accessibilityTraits.contains(.notEnabled))
        #expect(middle.accessibilityActivate())
        let ticked = try await waitFor("603 by 1167, 240 dpi", in: sheet.view)
        #expect(await eventually { ticked.accessibilityTraits.contains(.selected) })
        let enabled = try await waitFor("Save", in: sheet.view)
        #expect(await eventually { !enabled.accessibilityTraits.contains(.notEnabled) })
        #expect(model.chosenScale == .native, "ticking changes nothing yet")
        #expect(activate(enabled))
        #expect(await eventually { model.chosenScale == .factor(1.5) })
        #expect(await eventually { model.sheet == nil })
        #expect(model.resolution == .factor(1.5))

        // Cancel leaves the choice as it was.
        let again = try await openSheet(model, in: lease)
        let fastest = try await waitFor("402 by 778, 160 dpi, Fastest", in: again.view)
        #expect(fastest.accessibilityActivate())
        let cancel = try await waitFor("Cancel", in: again.view)
        #expect(activate(cancel))
        #expect(await eventually { model.sheet == nil })
        #expect(model.chosenScale == .factor(1.5))
    }

    @Test func nativeIsKeptAsTheSymbol() async throws {
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let model = model(native: 2.3, chosen: .points)
        let sheet = try await openSheet(model, in: lease)
        let native = try await waitFor("924 by 1789, 368 dpi, Native", in: sheet.view)
        #expect(native.accessibilityActivate())
        let save = try await waitFor("Save", in: sheet.view)
        #expect(await eventually { !save.accessibilityTraits.contains(.notEnabled) })
        #expect(activate(save))
        #expect(await eventually { model.chosenScale == .native })
        #expect(model.displayOptions == DisplayOptions(chosenScale: .native))
    }
}
