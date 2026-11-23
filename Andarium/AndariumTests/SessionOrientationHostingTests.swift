// SPDX-License-Identifier: GPL-2.0-or-later

import AndariumCore
import Foundation
import Observation
import SwiftUI
import Testing
import UIKit
@testable import Andarium

/// The real scene root's orientation ownership (no interface lock, and the
/// guest host-driven in an iPad session whatever is presented) and deferred
/// edges, and the built bundle's portrait-only iPhone declaration. Only a
/// device shows how the system answers the swipes (docs/testing.md, "Edge
/// gestures"). The scene reads as one that does not fold: a folding
/// display's inner display rotates the guest as an iPad does, and
/// `FoldSessionHostingTests` has that rule.
@MainActor
@Suite(.serialized)
struct SessionOrientationHostingTests {
    private func eventually(_ condition: () -> Bool) async -> Bool {
        let deadline = ContinuousClock.now + .seconds(10)
        while !condition(), ContinuousClock.now < deadline {
            try? await Task.sleep(for: .milliseconds(50))
        }
        return condition()
    }

    private static var applicationState: String {
        switch UIApplication.shared.applicationState {
        case .active: "active"
        case .inactive: "inactive (a test or the system took it out of the foreground)"
        case .background: "in the background"
        @unknown default: "in an unknown state"
        }
    }

    @Test func theSceneRootNeverLocksTheInterfaceAndDrivesTheGuestOnlyOnAnIPad() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = lease.scene
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        controller.foldReader.stub = { nil }
        lease.window.rootViewController = controller
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        #expect(controller.supportedInterfaceOrientations == (phone ? .portrait : .all))
        #expect(!controller.prefersInterfaceOrientationLocked)
        // Full screen or in a window, in every phase of a session the guest is
        // the host's to rotate on an iPad and its own on an iPhone.
        for phase in [Phase.booting(.normal), .running(.normal), .running(.recovery)] {
            model.phase = phase
            #expect(await eventually { model.hostDrivesGuestRotation == !phone }, "\(phase)")
            try await Task.sleep(for: .milliseconds(300))
            #expect(model.hostDrivesGuestRotation == !phone, "\(phase)")
            #expect(!controller.prefersInterfaceOrientationLocked, "\(phase)")
            #expect(!scene.effectiveGeometry.isInterfaceOrientationLocked, "\(phase)")
        }
        model.phase = .home
        #expect(await eventually { !model.hostDrivesGuestRotation })
        #expect(!controller.prefersInterfaceOrientationLocked)
    }

    /// The guest follows the device's orientation on an iPhone (whose interface stays
    /// portrait), and a launch can ask the same of any device: the interface is then
    /// portrait-only and the guest its own to rotate, as on an iPhone.
    @Test func theGuestFollowsTheDeviceOnAnIPhoneAndALaunchCanAskItOfAnyDevice() async throws {
        let model = AppEntry.fromArguments().makeModel()
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        #expect(model.followsDeviceOrientation == phone)
        let lease = try await HostWindow.take()
        defer { lease.give() }
        model.followsDeviceOrientation = true
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        lease.window.rootViewController = controller
        #expect(controller.supportedInterfaceOrientations == .portrait)
        for phase in [Phase.booting(.normal), .running(.normal)] {
            model.phase = phase
            try await Task.sleep(for: .milliseconds(300))
            #expect(!model.hostDrivesGuestRotation, "\(phase)")
        }
    }

    /// Whatever a session presents (the side keys' tip is a popover, Settings
    /// a sheet, a notice an alert) leaves the policy as it was. The scene root
    /// once asked for an interface lock, which UIKit answered with
    /// "unspecified" while anything was presented, so the guest changed hands
    /// and back with every popover.
    @Test func noPresentationChangesWhoDrivesTheGuestsRotation() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let toggles = SheetSwitch()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = lease.scene
        let controller = AndariumHostingController(rootView: PresentingRoot(toggles: toggles), model: model)
        controller.foldReader.stub = { nil }
        lease.window.rootViewController = controller
        let phone = UIDevice.current.userInterfaceIdiom == .phone
        model.phase = .running(.normal)
        #expect(await eventually { model.hostDrivesGuestRotation == !phone })
        try await Task.sleep(for: .milliseconds(500))
        let history = RotationHistory(model)

        func settled(_ what: String) async throws {
            try await Task.sleep(for: .milliseconds(600))
            #expect(model.hostDrivesGuestRotation == !phone, "\(what)")
            #expect(!controller.prefersInterfaceOrientationLocked, "\(what)")
            #expect(!scene.effectiveGeometry.isInterfaceOrientationLocked, "\(what)")
        }

        func presentUIKit(_ name: String, _ presented: UIViewController) async throws {
            controller.present(presented, animated: true)
            #expect(await eventually { controller.presentedViewController === presented }, "\(name) did not appear")
            try await settled("while \(name) is presented")
            controller.dismiss(animated: true)
            #expect(await eventually { controller.presentedViewController == nil }, "\(name) did not go")
            try await settled("after \(name)")
        }

        let alert = UIAlertController(title: "A notice", message: nil, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "OK", style: .default))
        try await presentUIKit("an alert", alert)

        let sheet = UIViewController()
        sheet.modalPresentationStyle = .pageSheet
        try await presentUIKit("a sheet", sheet)

        if !phone {
            let popover = UIViewController()
            popover.modalPresentationStyle = .popover
            popover.preferredContentSize = CGSize(width: 240, height: 120)
            popover.popoverPresentationController?.sourceView = controller.view
            popover.popoverPresentationController?.sourceRect = CGRect(x: 120, y: 120, width: 1, height: 1)
            try await presentUIKit("a popover", popover)
        }

        toggles.sheet = true
        #expect(await eventually { controller.presentedViewController != nil }, "the SwiftUI sheet did not appear")
        try await settled("while a SwiftUI sheet is presented")
        toggles.sheet = false
        #expect(await eventually { controller.presentedViewController == nil }, "the SwiftUI sheet did not go")
        try await settled("after a SwiftUI sheet")

        if !phone {
            toggles.popover = true
            #expect(await eventually { controller.presentedViewController != nil }, "the SwiftUI popover did not appear")
            try await settled("while a SwiftUI popover is presented")
            toggles.popover = false
            #expect(await eventually { controller.presentedViewController == nil }, "the SwiftUI popover did not go")
            try await settled("after a SwiftUI popover")
        }

        #expect(history.values.allSatisfy { $0 == !phone }, "the policy was \(history.values)")
        model.phase = .home
    }

    @Test func theSceneRootDefersTheGuestsEdgesOnlyInANormalRun() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let scene = lease.scene
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        controller.foldReader.stub = { nil }
        lease.window.rootViewController = controller
        // Outside a run the root keeps SwiftUI's preference, which never
        // takes the top or the bottom edge from the system.
        let outside = controller.preferredScreenEdgesDeferringSystemGestures
        #expect(!outside.contains(.top) && !outside.contains(.bottom))
        model.phase = .running(.normal)
        let size = scene.effectiveGeometry.coordinateSpace.bounds.size
        let screen = scene.screen.bounds.size
        let fullScreen = abs(size.width - screen.width) < 0.5 && abs(size.height - screen.height) < 0.5
        if fullScreen {
            #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == .all })
            #expect(controller.childForScreenEdgesDeferringSystemGestures == nil)
        } else {
            try await Task.sleep(for: .milliseconds(300))
            #expect(controller.preferredScreenEdgesDeferringSystemGestures == outside)
        }
        model.phase = .running(.recovery)
        #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == outside })
        model.phase = .home
        #expect(await eventually { controller.preferredScreenEdgesDeferringSystemGestures == outside })
    }

    @Test func theGuestKeepsThreeFingersWhileItHasTheKeyboardFocus() {
        let view = GuestMetalView(frame: .zero, device: nil)
        #expect(view.editingInteractionConfiguration == .none)
    }

    @Test func theSceneRootTellsTheDriverWhenTheGeometryChanges() async throws {
        let entry = AppEntry.fromArguments()
        let model = entry.makeModel()
        let driver = StubDriver(plan: nil)
        model.driver = driver
        let lease = try await HostWindow.take()
        defer { lease.give() }
        let controller = AndariumHostingController(rootView: Color.black, model: model)
        controller.foldReader.stub = { nil }
        lease.window.rootViewController = controller
        #expect(await eventually { driver.geometryChanges > 0 })
        // Let the first layouts settle, then report a storm of geometry
        // updates within one turn: with the geometry where the last update
        // found it, the driver hears nothing of them (`SceneUpdateTests` moves
        // each input on its own).
        try await Task.sleep(for: .milliseconds(500))
        let before = driver.geometryChanges
        for _ in 0..<50 { controller.sceneGeometryDidChange() }
        try await Task.sleep(for: .milliseconds(300))
        #expect(driver.geometryChanges == before)
        // A phase change is the scene root's to tell the driver of, once.
        model.phase = .running(.normal)
        #expect(await eventually { driver.geometryChanges > before })
        try await Task.sleep(for: .milliseconds(200))
        #expect(driver.geometryChanges - before <= 2)
        model.phase = .home
    }

    @Test func theBundleAllowsOnlyPortraitOnIPhone() throws {
        // Read the declarations themselves: Bundle's device-qualified lookup
        // resolves orientation keys for the iPad running this test.
        let data = try Data(contentsOf: Bundle.main.bundleURL.appendingPathComponent("Info.plist"))
        let info = try #require(try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any])
        let orientations = info["UISupportedInterfaceOrientations~iphone"] ?? info["UISupportedInterfaceOrientations"]
        #expect(orientations as? [String] == ["UIInterfaceOrientationPortrait"])
        #expect((info["UISupportedInterfaceOrientations~ipad"] as? [String])?.count == 4)
    }
}

/// Every value `hostDrivesGuestRotation` has taken since the recorder was
/// made. A change is told on whichever thread made it, so the hop back to
/// the main actor reads the model again.
@MainActor
private final class RotationHistory {
    private let model: AppModel
    private(set) var values: [Bool]

    init(_ model: AppModel) {
        self.model = model
        values = [model.hostDrivesGuestRotation]
        observe()
    }

    private func observe() {
        withObservationTracking {
            _ = model.hostDrivesGuestRotation
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                guard let self else { return }
                values.append(model.hostDrivesGuestRotation)
                observe()
            }
        }
    }
}

@MainActor @Observable
private final class SheetSwitch {
    var sheet = false
    var popover = false
}

private struct PresentingRoot: View {
    let toggles: SheetSwitch

    var body: some View {
        @Bindable var toggles = toggles
        Color.black
            .sheet(isPresented: $toggles.sheet) { Text("A sheet") }
            .popover(isPresented: $toggles.popover) { Text("A popover").padding() }
    }
}
